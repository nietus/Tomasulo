// Simulador do Algoritmo de Tomasulo - versão ROB

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream> // istringstream, stringstream
#include <map>     // std::map
#include <queue>   // std::queue
#include <iomanip> // setw, left, right
#include <limits>  // std::numeric_limits (limpar buffer de entrada)
#include <cstdlib> // atoi

using namespace std;

// Define os tipos de instrução que o simulador consegue processar.
// Adicionar novos tipos aqui exigiria expandir a lógica de tratamento.
enum InstructionType {
    ADD,
    SUB,
    MUL,
    DIV,
    LOAD,
    STORE,
    INVALID // Para casos de erro ou instruções não reconhecidas.
};

// Estados possíveis para uma entrada no Reorder Buffer (ROB).
// Isso ajuda a rastrear o ciclo de vida de cada instrução.
enum ROBState {
    ROB_EMPTY,      // Indica que a entrada do ROB está disponível para uso.
    ROB_ISSUE,      // A instrução foi alocada no ROB, aguardando recursos ou operandos.
    ROB_EXECUTE,    // A instrução está sendo processada por uma unidade funcional.
    ROB_WRITERESULT,// O resultado foi calculado e escrito no ROB, aguardando o commit.
    // O estado de COMMIT é mais um evento; após ele, a entrada volta a ser ROB_EMPTY.
};

// Representa uma instrução individual, como lida do arquivo de entrada.
// Também armazena informações de timing dos estágios do pipeline.
struct Instruction {
    InstructionType type;        // Qual operação? (ex: ADD, LOAD).
    string dest;                 // Registrador de destino (ex: "F1"). No caso de STORE, guarda o offset.
    string src1;                 // Primeiro operando. Para LOAD, é o offset; para STORE, é o registrador com o dado.
    string src2;                 // Segundo operando. Para LOAD/STORE, é o registrador base.
    int issue = -1;              // Ciclo de emissão da instrução. -1 se ainda não emitida.
    int execComp = -1;           // Ciclo de conclusão da execução. -1 se não concluída.
    int writeResult = -1;        // Ciclo de escrita do resultado no ROB (via CDB). -1 se não escrito.
    int commitCycle = -1;        // Ciclo de commit da instrução. -1 se não cometido.
};

// Detalhes de uma entrada no Reorder Buffer (ROB).
// O ROB é fundamental para garantir a finalização em ordem e o tratamento de exceções.
struct ReorderBufferEntry {
    bool busy = false;              // Esta entrada do ROB está em uso?
    int instructionIndex = -1;      // Qual instrução (do vetor 'instructions') está aqui?
    InstructionType type = INVALID; // Tipo da instrução nesta entrada.
    ROBState state = ROB_EMPTY;     // Estado atual desta instrução no ROB.
    string destinationRegister;     // Nome do registrador de destino arquitetural (ex: "F1"). Vazio para STORE.
    int value = 0;                  // Resultado (ALU/LOAD) ou dado a ser armazenado (STORE).
    int address = 0;                // Endereço de memória (para LOAD/STORE) após cálculo.
    bool valueReady = false;        // O campo 'value' (resultado/dado do store) já está disponível?
};

// Representa uma Estação de Reserva (RS).
// As RSs contêm instruções que aguardam operandos ou a disponibilidade de UFs.
struct ReservationStation {
    bool busy = false;            // Esta RS está ocupada?
    InstructionType op = INVALID; // Operação sendo processada ou aguardada.
    int Vj = 0, Vk = 0;           // Valores dos operandos fonte. Vj para src1, Vk para src2/base.
    string Qj = "", Qk = "";      // Tags (índices do ROB) que produzirão Vj e Vk.
                                  // Se vazias, Vj/Vk estão prontos.
    string destRobIndex = "";     // Para qual entrada do ROB esta RS enviará o resultado.
    int A = 0;                    // Campo para offset em instruções L.D/S.D.
    int instructionIndex = -1;    // Índice da instrução original associada a esta RS.
};

// Status de um registrador arquitetural, essencial para a renomeação.
// Indica se o valor do registrador está pendente e qual entrada do ROB o produzirá.
struct RegisterStatus {
    bool busy = false;          // True se o registrador aguarda um resultado de uma entrada do ROB.
    int robIndex = -1;          // Índice da entrada do ROB que fornecerá o valor.
                                // -1 indica que o valor está no banco de registradores, não pendente.
};

// Classe principal do simulador, encapsula toda a lógica e os componentes.
class TomasuloSimulator {
private:
    // --- Componentes Centrais do Processador Simulado ---
    vector<Instruction> instructions;           // Armazena todas as instruções do programa.
    vector<ReservationStation> addRS, mulRS, loadRS, storeRS; // Grupos de Estações de Reserva, por tipo.
    vector<ReorderBufferEntry> rob;             // O Reorder Buffer.
    map<string, int> registers;                 // Simula o banco de registradores arquiteturais.
    map<string, RegisterStatus> regStatus;      // Tabela de status dos registradores para renomeação.
    int memory[1024];                           // Simulação da memória principal.

    // --- Variáveis para Controle da Simulação ---
    int cycle = 0;                          // Contador de ciclos da simulação.
    int nextInstructionIndex = 0;           // Aponta para a próxima instrução a ser emitida do programa.
    int robHead = 0, robTail = 0;           // Ponteiros para a fila circular do ROB (cabeça e cauda).
    int robEntriesAvailable;                // Contador de entradas livres no ROB.

    // --- Parâmetros de Configuração do Simulador ---
    // Latências das unidades funcionais. Poderiam ser configuráveis.
    const int ADD_LATENCY, MUL_LATENCY, DIV_LATENCY, LOAD_LATENCY, STORE_LATENCY;
    const int ROB_SIZE; // Tamanho do Reorder Buffer.

    // --- Estruturas Auxiliares para a Fase de Execução ---
    // Mantém o controle de instruções que estão atualmente nas unidades funcionais.
    // Ou seja, já saíram da RS, mas ainda não enviaram resultado pelo CDB.
    struct ExecutingInstruction {
        int rsIndex;          // Índice da RS de origem.
        string rsType;        // Tipo da RS (ADD, MUL, etc.) para identificação.
        int remainingCycles;  // Quantos ciclos faltam para completar a execução.
        int instructionIndex; // Índice da instrução original.
    };
    vector<ExecutingInstruction> executingInstructions;

    // Fila para instruções que finalizaram a execução e aguardam o CDB.
    // Os índices são da lista original de instruções.
    queue<int> completedForCDB;

    // Busca uma Estação de Reserva (RS) livre para o tipo de instrução especificado.
    // Retorna o índice da RS e uma string identificando o tipo de RS.
    pair<int, string> findFreeRS(InstructionType type) {
        if (type == ADD || type == SUB) { // ADD e SUB usam as mesmas RSs.
            for (size_t i = 0; i < addRS.size(); ++i) if (!addRS[i].busy) return {static_cast<int>(i), "ADD"};
        } else if (type == MUL || type == DIV) { // MUL e DIV usam as mesmas RSs.
            for (size_t i = 0; i < mulRS.size(); ++i) if (!mulRS[i].busy) return {static_cast<int>(i), "MUL"};
        } else if (type == LOAD) {
            for (size_t i = 0; i < loadRS.size(); ++i) if (!loadRS[i].busy) return {static_cast<int>(i), "LOAD"};
        } else if (type == STORE) {
            for (size_t i = 0; i < storeRS.size(); ++i) if (!storeRS[i].busy) return {static_cast<int>(i), "STORE"};
        }
        return {-1, ""}; // Sinaliza que nenhuma RS do tipo está livre.
    }

    // Lógica do estágio de Emissão (Issue).
    // Tenta alocar recursos (ROB, RS) e buscar operandos para a próxima instrução.
    bool issueInstruction() {
        // Condições para não emitir: sem instruções pendentes ou ROB cheio.
        if (nextInstructionIndex >= static_cast<int>(instructions.size()) || robEntriesAvailable == 0) {
            return false; // Não há o que ou onde emitir.
        }
        Instruction &originalInst = instructions[nextInstructionIndex]; // Referência à instrução a ser emitida.

        // Verifica disponibilidade de RS.
        pair<int, string> rsInfo = findFreeRS(originalInst.type);
        if (rsInfo.first == -1) {
            return false; // Emperramento estrutural: nenhuma RS livre para este tipo de instrução.
        }

        // Passo 1: Alocar entrada no ROB.
        int currentRobIdx = robTail; // Pega a próxima posição livre na cauda do ROB.
        ReorderBufferEntry &robEntry = rob[currentRobIdx];
        robEntry.busy = true;
        robEntry.instructionIndex = nextInstructionIndex;
        robEntry.type = originalInst.type;
        robEntry.state = ROB_ISSUE; // Estado inicial da instrução no ROB.
        // STORE não tem registrador de destino arquitetural; 'dest' em STORE é o offset.
        robEntry.destinationRegister = (originalInst.type != STORE) ? originalInst.dest : "";
        robEntry.value = 0; // Inicializa valor.
        robEntry.address = 0; // Inicializa endereço.
        robEntry.valueReady = false; // Valor ainda não está pronto.

        robTail = (robTail + 1) % ROB_SIZE; // Avança a cauda do ROB (circular).
        robEntriesAvailable--;
        originalInst.issue = cycle; // Marca o ciclo de emissão na instrução original.

        // Passo 2: Preencher a Estação de Reserva (RS) que foi alocada.
        ReservationStation *rs = nullptr; // Ponteiro para a RS específica.
        if (rsInfo.second == "ADD") rs = &addRS[rsInfo.first];
        else if (rsInfo.second == "MUL") rs = &mulRS[rsInfo.first];
        else if (rsInfo.second == "LOAD") rs = &loadRS[rsInfo.first];
        else if (rsInfo.second == "STORE") rs = &storeRS[rsInfo.first];
        // Se rsInfo.first != -1, rs não deveria ser nullptr aqui.

        rs->busy = true;
        rs->op = originalInst.type;
        rs->instructionIndex = nextInstructionIndex;
        // A RS precisa saber para qual entrada do ROB ela deve enviar seu resultado.
        rs->destRobIndex = to_string(currentRobIdx);

        // Passo 3: Obter operandos (Vj, Vk) ou as tags de dependência (Qj, Qk) para a RS.
        // Tratamento do primeiro operando (src1 -> Vj/Qj).
        if (originalInst.type == LOAD) { // Para LOAD, src1 é o offset, vai para o campo 'A'.
            rs->A = atoi(originalInst.src1.c_str());
            rs->Vj = 0; rs->Qj = ""; // Vj/Qj não são usados para registrador em LOAD desta forma.
        } else { // Para ADD, SUB, MUL, DIV (src1 é um registrador) e STORE (src1 é o registrador do dado).
            if (!originalInst.src1.empty()) { // src1 existe?
                if (regStatus[originalInst.src1].busy) { // Valor de src1 está pendente?
                    int producingRobIdx = regStatus[originalInst.src1].robIndex;
                    // O valor já pode estar pronto no ROB, mesmo que o commit não tenha ocorrido.
                    // Isso permite pegar o valor adiantado (forwarding).
                    if (rob[producingRobIdx].busy && rob[producingRobIdx].state == ROB_WRITERESULT && rob[producingRobIdx].valueReady) {
                        rs->Vj = rob[producingRobIdx].value; // Pega valor direto do ROB.
                        rs->Qj = ""; // Marca como disponível.
                    } else {
                        rs->Qj = to_string(producingRobIdx); // Ainda não pronto, armazena a tag do ROB.
                    }
                } else { // Valor de src1 está no banco de registradores.
                    rs->Vj = registers[originalInst.src1];
                    rs->Qj = ""; // Marca como disponível.
                }
            } else { rs->Vj = 0; rs->Qj = "";} // Caso não haja src1 (raro, depende da arquitetura).
        }

        // Tratamento do segundo operando (src2 -> Vk/Qk).
        // Aplica-se a Arith (src2 é registrador) e Load/Store (src2 é registrador base).
        if (originalInst.type == ADD || originalInst.type == SUB || originalInst.type == MUL || originalInst.type == DIV ||
            originalInst.type == LOAD || originalInst.type == STORE) { // Instruções que podem usar src2.
            if (!originalInst.src2.empty()) { // src2 existe?
                if (regStatus[originalInst.src2].busy) { // Valor de src2 pendente?
                    int producingRobIdx = regStatus[originalInst.src2].robIndex;
                    if (rob[producingRobIdx].busy && rob[producingRobIdx].state == ROB_WRITERESULT && rob[producingRobIdx].valueReady) {
                        rs->Vk = rob[producingRobIdx].value; // Pega valor direto do ROB.
                        rs->Qk = ""; // Marca como disponível.
                    } else {
                        rs->Qk = to_string(producingRobIdx); // Ainda não pronto, armazena tag.
                    }
                } else { // Valor de src2 no banco de registradores.
                    rs->Vk = registers[originalInst.src2];
                    rs->Qk = ""; // Marca como disponível.
                }
            } else { // Sem src2 explícito (ex: L.D F1, 100() poderia implicar base R0 ou ser um erro de formato).
                     // Neste simulador, assume-se 0 se src2 estiver vazio.
                rs->Vk = 0;
                rs->Qk = "";
            }
        } else {rs->Vk = 0; rs->Qk = "";} // Instruções que não usam um segundo operando registrador.

        // Lógica específica para STORE durante o Issue.
        if (originalInst.type == STORE) {
            // Para STORE, 'inst.dest' (que vem do parsing de offset(base)) é o offset.
            rs->A = atoi(originalInst.dest.c_str());
            // Se o valor a ser armazenado (Vj, vindo de inst.src1) já estiver disponível na RS,
            // o campo 'value' e 'valueReady' na entrada do ROB do STORE pode ser preenchido.
            if (rs->Qj.empty()) { // Vj (dado do store) está pronto?
                rob[currentRobIdx].value = rs->Vj; // 'value' no ROB do STORE é o dado a ser escrito.
                rob[currentRobIdx].valueReady = true;
            }
        }

        // Passo 4: Atualizar a Tabela de Status do Registrador de Destino (Renomeação).
        // Se a instrução modifica um registrador (ou seja, não é STORE),
        // marca esse registrador como 'busy' e aponta para a entrada do ROB que calculará seu novo valor.
        if (originalInst.type != STORE) {
            regStatus[originalInst.dest].busy = true;
            regStatus[originalInst.dest].robIndex = currentRobIdx;
        }

        nextInstructionIndex++; // Avança para a próxima instrução do programa.
        return true; // Emissão bem-sucedida.
    }

    // Dispara a execução de instruções nas RSs que têm todos os operandos prontos (Qj e Qk vazios).
    void startExecution() {
        // Agrupa as RSs para facilitar a iteração.
        ReservationStation *rs_arrays[] = {addRS.data(), mulRS.data(), loadRS.data(), storeRS.data()};
        size_t rs_sizes[] = {addRS.size(), mulRS.size(), loadRS.size(), storeRS.size()};
        string rs_types[] = {"ADD", "MUL", "LOAD", "STORE"};
        // Latência base para cada tipo. MUL/DIV é tratado separadamente devido à mesma RS.
        int base_latencies[] = {ADD_LATENCY, 0, LOAD_LATENCY, STORE_LATENCY};

        for (int type_idx = 0; type_idx < 4; ++type_idx) { // Itera sobre os tipos de RS (ADD, MUL, LOAD, STORE).
            for (size_t i = 0; i < rs_sizes[type_idx]; ++i) { // Itera sobre cada RS do tipo atual.
                ReservationStation &currentRS = rs_arrays[type_idx][i];
                // Verifica se a RS está ocupada e se todos os operandos estão disponíveis.
                if (currentRS.busy && currentRS.Qj.empty() && currentRS.Qk.empty()) {
                    bool alreadyExecuting = false;
                    // Garante que a instrução desta RS não foi enviada para execução anteriormente.
                    for (const auto& execInst : executingInstructions) {
                        if (execInst.rsType == rs_types[type_idx] && execInst.rsIndex == static_cast<int>(i)) {
                            alreadyExecuting = true; break;
                        }
                    }

                    if (!alreadyExecuting) { // Se não está executando, pode começar.
                        int robIdxForInst = stoi(currentRS.destRobIndex); // Obtém o índice do ROB associado.
                        // Atualiza o estado da instrução no ROB para EXECUTE.
                        // É importante verificar se a entrada do ROB ainda é relevante (busy e no estado ISSUE).
                        if(rob[robIdxForInst].busy && rob[robIdxForInst].state == ROB_ISSUE) {
                            rob[robIdxForInst].state = ROB_EXECUTE;
                        }

                        ExecutingInstruction exec; // Prepara para adicionar à lista de instruções em execução.
                        exec.rsIndex = i;
                        exec.rsType = rs_types[type_idx];
                        exec.instructionIndex = currentRS.instructionIndex;

                        // Define a latência correta. MUL e DIV compartilham RSs, mas têm latências diferentes.
                        if (rs_types[type_idx] == "MUL") {
                            exec.remainingCycles = (currentRS.op == MUL) ? MUL_LATENCY : DIV_LATENCY;
                        } else {
                            exec.remainingCycles = base_latencies[type_idx];
                        }
                        executingInstructions.push_back(exec); // Adiciona à lista de execução.

                        // Caso especial para STORE: se o valor (Vj) ficou pronto (Qj resolvido)
                        // e a instrução está começando a execução (Qk também resolvido),
                        // o valor do dado do STORE deve ser atualizado na entrada do ROB.
                        if (currentRS.op == STORE && rob[robIdxForInst].busy && !rob[robIdxForInst].valueReady) {
                            rob[robIdxForInst].value = currentRS.Vj; // Vj deve estar pronto neste ponto.
                            rob[robIdxForInst].valueReady = true;
                        }
                    }
                }
            }
        }
    }

    // Simula o avanço de um ciclo para as instruções que estão nas unidades funcionais.
    void advanceExecution() {
        // Iterar e potencialmente remover elementos requer cuidado com o índice.
        for (size_t i = 0; i < executingInstructions.size(); /* incremento manual abaixo */) {
            executingInstructions[i].remainingCycles--; // Decrementa o contador de ciclos restantes.
            if (executingInstructions[i].remainingCycles <= 0) { // Execução concluída.
                // Registra o ciclo de conclusão da execução na instrução original.
                instructions[executingInstructions[i].instructionIndex].execComp = cycle;
                // Adiciona à fila do CDB para escrita no ROB no próximo ciclo.
                completedForCDB.push(executingInstructions[i].instructionIndex);
                // Remove da lista de instruções em execução.
                executingInstructions.erase(executingInstructions.begin() + i);
                // Não incrementa 'i' aqui, pois o erase desloca os elementos.
            } else {
                i++; // Avança para a próxima instrução em execução.
            }
        }
    }

    // Processa os resultados que chegam pelo Common Data Bus (CDB).
    // Escreve o resultado na entrada do ROB e encaminha para RSs dependentes.
    void processWriteBack() {
        if (completedForCDB.empty()) return; // Fila do CDB vazia, nada a fazer.

        int originalInstIndex = completedForCDB.front(); // Pega a próxima instrução da fila.
        completedForCDB.pop(); // Remove da fila.

        Instruction &inst = instructions[originalInstIndex]; // Referência à instrução original.
        // Registra o ciclo em que o resultado é escrito no ROB.
        inst.writeResult = cycle;

        ReservationStation *rs = nullptr; // Ponteiro para a RS que originou esta instrução.
        string robIndexStr; // String contendo o índice do ROB de destino.

        // Localiza a RS que processou esta instrução para obter operandos e liberar a RS.
        // Este passo é crucial para saber de onde vieram os Vj, Vk, A.
        bool found_rs = false;
        // A busca poderia ser otimizada se a RS guardasse um ponteiro/ID da instrução em execução.
        if (!found_rs) for (size_t i = 0; i < addRS.size(); ++i) if (addRS[i].instructionIndex == originalInstIndex && addRS[i].busy) { rs = &addRS[i]; robIndexStr = rs->destRobIndex; found_rs = true; break;}
        if (!found_rs) for (size_t i = 0; i < mulRS.size(); ++i) if (mulRS[i].instructionIndex == originalInstIndex && mulRS[i].busy) { rs = &mulRS[i]; robIndexStr = rs->destRobIndex; found_rs = true; break;}
        if (!found_rs) for (size_t i = 0; i < loadRS.size(); ++i) if (loadRS[i].instructionIndex == originalInstIndex && loadRS[i].busy) { rs = &loadRS[i]; robIndexStr = rs->destRobIndex; found_rs = true; break;}
        if (!found_rs) for (size_t i = 0; i < storeRS.size(); ++i) if (storeRS[i].instructionIndex == originalInstIndex && storeRS[i].busy) { rs = &storeRS[i]; robIndexStr = rs->destRobIndex; found_rs = true; break;}

        if (rs == nullptr || robIndexStr.empty()) {
            // Isso pode ocorrer se a instrução foi, por exemplo, "squashed" por um branch mal predito (não simulado aqui)
            // ou se já foi cometida e a RS liberada por algum motivo de timing.
            // Para este simulador, geralmente indica um estado inesperado ou uma condição de corrida sutil.
            // Um log de alerta pode ser útil aqui para depuração.
            // Ex: cout << "Alerta no WriteBack: RS para inst " << originalInstIndex << " não encontrada..." << endl;
            return; // Não pode prosseguir sem a RS ou o índice do ROB.
        }
        int producingRobIdx = stoi(robIndexStr); // Converte a string do índice do ROB para inteiro.

        int resultData = 0;      // Para resultados de ALU e dados de LOAD.
        int effectiveAddr = 0;   // Para o endereço calculado em LOAD/STORE.

        // Calcula o resultado ou endereço efetivo, com base no tipo da instrução.
        // Os valores Vj, Vk, A são lidos da RS onde a instrução estava aguardando.
        switch (inst.type) {
            case ADD: resultData = rs->Vj + rs->Vk; break;
            case SUB: resultData = rs->Vj - rs->Vk; break;
            case MUL: resultData = rs->Vj * rs->Vk; break;
            case DIV:
                if (rs->Vk != 0) resultData = rs->Vj / rs->Vk;
                else { /* Tratamento de divisão por zero. */ cerr << "Erro: Divisao por zero na instrucao " << originalInstIndex << "!" << endl; resultData = 0; }
                break;
            case LOAD:
                effectiveAddr = rs->A + rs->Vk; // A (offset) + Vk (valor do registrador base).
                if (effectiveAddr >= 0 && effectiveAddr < 1024) resultData = memory[effectiveAddr]; // Simula leitura da memória.
                else { /* Tratamento de acesso inválido à memória. */ cerr << "Erro: Endereco de LOAD invalido (" << effectiveAddr << ") para inst " << originalInstIndex << endl; resultData = 0; }
                rob[producingRobIdx].address = effectiveAddr; // Armazena o endereço calculado na entrada do ROB.
                break;
            case STORE:
                effectiveAddr = rs->A + rs->Vk;
                rob[producingRobIdx].address = effectiveAddr; // Armazena o endereço calculado.
                // O valor a ser armazenado (rs->Vj) já deveria estar em rob[producingRobIdx].value
                // se foi obtido no Issue ou atualizado por updateDependentRS.
                // Aqui, 'resultData' para STORE efetivamente é o valor que estava em Vj.
                resultData = rs->Vj;
                break;
            case INVALID: // Caso de instrução inválida detectada tardiamente.
                cerr << "Erro: Instrução inválida no WriteBack para índice " << originalInstIndex << endl;
                resultData = 0; // Define um valor padrão para não deixar lixo.
                // Considerar se o simulador deve parar ou sinalizar erro de forma mais forte.
                return; // Retornar aqui evita a atualização do ROB e a liberação da RS para uma instrução inválida.
        }

        // Atualiza a entrada correspondente no ROB com o resultado/valor e muda o estado.
        if (rob[producingRobIdx].busy) { // Verifica se a entrada do ROB ainda é relevante (não foi liberada).
            rob[producingRobIdx].value = resultData;      // Armazena o resultado (ALU/LOAD) ou o dado (STORE).
            rob[producingRobIdx].valueReady = true;       // Marca que o valor está pronto.
            rob[producingRobIdx].state = ROB_WRITERESULT; // Pronta para ser considerada pelo Commit.

            // Transmite o resultado (valor e o ÍNDICE DO ROB que o produziu) para as RSs dependentes.
            // Isso é o "forwarding" através do CDB.
            updateDependentRS(producingRobIdx, resultData);
        }

        // Libera a Estação de Reserva, tornando-a disponível para novas instruções.
        rs->busy = false;
        rs->instructionIndex = -1; // Limpa associação com instrução.
        rs->Qj = ""; rs->Qk = ""; rs->Vj = 0; rs->Vk = 0; rs->A = 0; rs->destRobIndex = ""; // Reseta campos.
    }

    // Percorre todas as RSs para atualizar aquelas que esperavam por um resultado
    // que acabou de ser disponibilizado no CDB (identificado por 'producingRobIdx').
    void updateDependentRS(int producingRobIdx, int resultValue) {
        string producingTag = to_string(producingRobIdx); // Tag do ROB que produziu o resultado.
        // Agrupa as RSs para facilitar a iteração.
        ReservationStation *rs_arrays[] = {addRS.data(), mulRS.data(), loadRS.data(), storeRS.data()};
        size_t rs_sizes[] = {addRS.size(), mulRS.size(), loadRS.size(), storeRS.size()};

        for (int type_idx = 0; type_idx < 4; ++type_idx) { // Itera sobre os tipos de RS.
            for (size_t i = 0; i < rs_sizes[type_idx]; ++i) { // Itera sobre cada RS.
                ReservationStation &currentRS = rs_arrays[type_idx][i];
                if (currentRS.busy) { // Apenas RSs ocupadas podem estar esperando.
                    // Verifica se o operando Qj estava aguardando esta tag.
                    if (currentRS.Qj == producingTag) {
                        currentRS.Vj = resultValue; // Fornece o valor.
                        currentRS.Qj = "";          // Limpa a tag de espera, operando agora está pronto.

                        // Tratamento especial para STORE: se Qj era o operando de DADO (fonte do valor a ser armazenado),
                        // e esse valor acabou de ficar pronto, ele precisa ser propagado para a entrada do ROB do STORE.
                        if (currentRS.op == STORE) {
                            int storeOwnRobIdx = stoi(currentRS.destRobIndex); // Índice do ROB do próprio STORE.
                            // Verifica se a entrada do ROB do STORE ainda é válida.
                            if (rob[storeOwnRobIdx].busy) {
                                rob[storeOwnRobIdx].value = resultValue; // Atualiza o valor a ser armazenado.
                                rob[storeOwnRobIdx].valueReady = true;   // Marca que o dado está pronto.
                            }
                        }
                    }
                    // Verifica se o operando Qk estava aguardando esta tag.
                    if (currentRS.Qk == producingTag) {
                        currentRS.Vk = resultValue; // Fornece o valor.
                        currentRS.Qk = "";          // Limpa a tag de espera.
                    }
                    // Se ambos Qj e Qk agora estão vazios, esta RS pode se tornar candidata a `startExecution` no próximo ciclo.
                }
            }
        }
    }

    // Lógica do estágio de Commit.
    // Efetiva o resultado da instrução na cabeça do ROB no estado arquitetural (registradores ou memória).
    // Garante a finalização em ordem das instruções.
    void commitInstruction() {
        // Condições para não cometer: ROB vazio ou a entrada na cabeça não está ocupada/pronta.
        if (robEntriesAvailable == ROB_SIZE || !rob[robHead].busy) return;

        ReorderBufferEntry &headEntry = rob[robHead]; // Referência à entrada na cabeça do ROB.

        // Uma instrução só pode ser cometida se seu resultado já foi escrito no ROB (estado ROB_WRITERESULT).
        if (headEntry.state == ROB_WRITERESULT) {
            // Condição adicional para STORE: o VALOR a ser armazenado DEVE estar pronto.
            // O endereço pode estar pronto (estado WriteResult), mas o dado (value) não.
            if (headEntry.type == STORE && !headEntry.valueReady) {
                // Se um STORE está na cabeça do ROB, mas seu dado ainda não está pronto
                // (ex: dependia de um LOAD longo), ele bloqueia o commit de todas as instruções subsequentes.
                // Este é um ponto crucial para a corretude da memória com STOREs.
                return; // Não pode cometer este STORE ainda.
            }

            Instruction &originalInst = instructions[headEntry.instructionIndex]; // Referência à instrução original.
            originalInst.commitCycle = cycle; // Registra o ciclo de commit.

            string committedActionLog; // String para log do que foi efetivado.

            // Efetiva a escrita no estado arquitetural.
            if (headEntry.type != STORE) { // Para ADD, SUB, MUL, DIV, LOAD: atualiza registrador.
                registers[headEntry.destinationRegister] = headEntry.value;
                committedActionLog = headEntry.destinationRegister + " = " + to_string(headEntry.value);
                // Libera o status do registrador de destino se esta entrada do ROB
                // era a última que estava produzindo valor para ele.
                if (regStatus[headEntry.destinationRegister].busy &&
                    regStatus[headEntry.destinationRegister].robIndex == robHead) {
                    regStatus[headEntry.destinationRegister].busy = false;
                    regStatus[headEntry.destinationRegister].robIndex = -1; // Registrador agora tem valor atualizado.
                }
            } else { // Para STORE: atualiza memória.
                // É importante verificar a validade do endereço antes de escrever.
                if (headEntry.address >= 0 && headEntry.address < 1024) { // Limites da memória simulada.
                    memory[headEntry.address] = headEntry.value;
                    committedActionLog = "MEM[" + to_string(headEntry.address) + "] = " + to_string(headEntry.value);
                } else {
                    // Erro grave: tentativa de escrita em endereço inválido no commit.
                    // O comportamento aqui (logar, parar, etc.) depende dos requisitos.
                    cerr << "Erro CRITICO no Commit: Endereco de STORE invalido: " << headEntry.address << " para inst " << headEntry.instructionIndex << endl;
                    committedActionLog = "STORE ERRO - Endereco Invalido";
                    // Por ora, a instrução é "cometida" (ROB liberado) mas com o erro logado.
                    // Em um sistema real, isso poderia gerar uma exceção.
                }
            }

            // Log da ação de commit para depuração/visualização.
            cout << "Ciclo " << cycle << ": Commit Inst " << headEntry.instructionIndex << " (ROB " << robHead << "): " << committedActionLog << endl;

            // Libera a entrada do ROB.
            headEntry.busy = false;
            headEntry.state = ROB_EMPTY; // Marca como vazia para reutilização.
            robHead = (robHead + 1) % ROB_SIZE; // Avança a cabeça do ROB (circular).
            robEntriesAvailable++;
        }
        // Se headEntry.state não for ROB_WRITERESULT, a cabeça do ROB está bloqueada,
        // e nenhuma instrução (mesmo as prontas subsequentes) pode ser cometida devido à política de commit em ordem.
    }

public:
    // Construtor. Inicializa o simulador com os tamanhos das estruturas e latências.
    // Valores padrão são fornecidos se nenhum argumento for passado.
    TomasuloSimulator(int addRSCount = 3, int mulRSCount = 2, int loadRSCount = 3, int storeRSCount = 3, int rob_s = 16) :
        // Inicialização das latências (poderiam ser parâmetros do construtor também).
        ADD_LATENCY(2), MUL_LATENCY(10), DIV_LATENCY(40), LOAD_LATENCY(2), STORE_LATENCY(2),
        ROB_SIZE(rob_s), rob(rob_s) // Inicializa o ROB com o tamanho especificado.
    {
        // Redimensiona os vetores de Estações de Reserva.
        addRS.resize(addRSCount);
        mulRS.resize(mulRSCount);
        loadRS.resize(loadRSCount);
        storeRS.resize(storeRSCount);

        // Configurações iniciais do ROB e controle.
        robHead = 0;
        robTail = 0;
        robEntriesAvailable = ROB_SIZE; // ROB começa vazio.

        // Inicializa os registradores arquiteturais (F0-F31) com um valor padrão (10)
        // e marca seus status como livres (não pendentes).
        for (int i = 0; i < 32; i++) {
            string regName = "F" + to_string(i);
            registers[regName] = 10; // Valor inicial arbitrário.
            regStatus[regName] = RegisterStatus(); // busy=false, robIndex=-1 por padrão.
        }
        // Inicializa a memória com valores previsíveis (memory[i] = i) para facilitar a verificação.
        for (int i = 0; i < 1024; i++) memory[i] = i;

        // Reseta contadores de simulação.
        cycle = 0;
        nextInstructionIndex = 0;
    }

    // Carrega as instruções de um arquivo de texto.
    // Retorna true se bem-sucedido, false caso contrário.
    bool loadInstructions(const string &filename) {
        ifstream file(filename.c_str()); // Tenta abrir o arquivo.
        if (!file.is_open()) {
            cerr << "Erro ao abrir arquivo: " << filename << endl;
            return false;
        }
        string line; // Para ler cada linha do arquivo.
        // Processa linha por linha.
        while (getline(file, line)) {
            if (line.empty() || line[0] == '#') continue; // Ignora linhas vazias ou comentários no arquivo de entrada.

            istringstream iss(line); // Para facilitar o parsing da linha.
            string op, p1, p2, p3;   // Tokens temporários para os operandos.
            iss >> op >> p1;         // Lê a operação e o primeiro parâmetro.
            if (!p1.empty() && p1.back() == ',') p1.pop_back(); // Remove vírgula, se houver.

            Instruction inst; // Cria uma nova estrutura de instrução.
            // Parseia a instrução com base no mnemônico da operação.
            if (op == "ADD" || op == "SUB" || op == "MUL" || op == "DIV") { // Instruções aritméticas.
                if (op == "ADD") inst.type = ADD;
                else if (op == "SUB") inst.type = SUB;
                else if (op == "MUL") inst.type = MUL;
                else if (op == "DIV") inst.type = DIV;
                iss >> p2 >> p3; // Lê os outros dois operandos.
                if (!p2.empty() && p2.back() == ',') p2.pop_back(); // Remove vírgula.
                inst.dest = p1; inst.src1 = p2; inst.src2 = p3;
            } else if (op == "L.D" || op == "LOAD") { // Instrução de Load.
                inst.type = LOAD;
                inst.dest = p1; // p1 é o registrador de destino.
                iss >> p2;      // p2 é a string "offset(baseReg)".
                // Extrai offset e registrador base da string p2.
                size_t openParen = p2.find('('), closeParen = p2.find(')');
                if (openParen != string::npos && closeParen != string::npos && closeParen > openParen + 1) {
                    inst.src1 = p2.substr(0, openParen); // Offset.
                    inst.src2 = p2.substr(openParen + 1, closeParen - openParen - 1); // Registrador base.
                } else { /* Formato inválido. */ cerr << "Formato L.D invalido: " << p2 << " na linha: "<< line << endl; continue; }
            } else if (op == "S.D" || op == "STORE") { // Instrução de Store.
                inst.type = STORE;
                inst.src1 = p1; // p1 é o registrador do dado a ser armazenado.
                iss >> p2;      // p2 é "offset(baseReg)".
                // Extrai offset e registrador base.
                size_t openParen = p2.find('('), closeParen = p2.find(')');
                if (openParen != string::npos && closeParen != string::npos && closeParen > openParen +1) {
                    // Em STORE, o campo 'dest' da instrução é usado para guardar o offset.
                    inst.dest = p2.substr(0, openParen);
                    inst.src2 = p2.substr(openParen + 1, closeParen - openParen - 1); // Registrador base.
                } else { /* Formato inválido. */ cerr << "Formato S.D invalido: " << p2 << " na linha: " << line << endl; continue; }
            } else { // Operação não reconhecida.
                cerr << "Instrucao nao reconhecida: " << op << " na linha: " << line << endl;
                continue; // Pula para a próxima linha.
            }
            instructions.push_back(inst); // Adiciona a instrução parseada ao vetor.
        }
        file.close(); // Fecha o arquivo.
        return true; // Carregamento bem-sucedido.
    }

    // Verifica se a simulação chegou ao fim.
    // Condições: todas as instruções emitidas, ROB vazio, nenhuma instrução executando ou na fila do CDB.
    bool isSimulationComplete() const {
        // Ainda há instruções do programa para serem emitidas?
        if (nextInstructionIndex < static_cast<int>(instructions.size())) return false;
        // O ROB não está completamente vazio (ou seja, nem todas as entradas estão disponíveis)?
        if (robEntriesAvailable != ROB_SIZE) return false;
        // Ainda há instruções em unidades funcionais ou aguardando o CDB para escrever?
        if (!executingInstructions.empty() || !completedForCDB.empty()) return false;

        // Verificação opcional, mais rigorosa: todas as instruções no vetor 'instructions'
        // devem ter um 'commitCycle' válido. As condições acima geralmente são suficientes
        // se o programa não estiver vazio.
        if (!instructions.empty()) { // Só faz sentido verificar se há instruções.
            for(const auto &inst : instructions) {
                if(inst.commitCycle == -1) return false; // Se alguma instrução não foi cometida.
            }
        }
        return true; // Se todas as condições de término foram satisfeitas.
    }

    // Avança um ciclo da simulação, executando as fases do pipeline na ordem correta.
    // A ordem é importante para o fluxo de dados e controle.
    void stepSimulation() {
        // 1. Commit: Tenta cometer a instrução na cabeça do ROB.
        //    Isso libera entradas do ROB e atualiza o estado arquitetural.
        //    Deve vir antes do Issue para que o Issue possa usar entradas do ROB recém-liberadas.
        commitInstruction();

        // 2. WriteResult (CDB->ROB): Resultados das UFs são escritos no ROB
        //    e transmitidos via CDB para RSs dependentes.
        //    Libera RSs e disponibiliza operandos. Deve vir antes do Execute/Issue
        //    para que instruções possam começar a executar ou ser emitidas com operandos atualizados.
        processWriteBack();

        // 3. Issue: Novas instruções são alocadas no ROB e nas RSs.
        //    Pode usar recursos (ROB, tags) que foram atualizados/liberados
        //    pelas fases de Commit e WriteResult deste ciclo.
        issueInstruction();

        // 4. Execute (Start & Advance): Instruções com operandos prontos
        //    (potencialmente devido ao WriteResult deste mesmo ciclo)
        //    iniciam ou continuam sua execução. O estado no ROB é atualizado para EXECUTE.
        startExecution();   // Verifica RSs prontas e as move para a lista de execução.
        advanceExecution(); // Decrementa contadores das instruções em execução.

        cycle++; // Avança o contador de ciclo global.
    }

    // Retorna o ciclo atual da simulação.
    int getCurrentCycle() const { return cycle; }

    // Imprime os valores finais dos registradores arquiteturais.
    // Útil ao final da simulação para verificar os resultados.
    void printRegisters() const {
        cout << "\nValores Finais dos Registradores:\n---------------------------------\n";
        // Usar range-based for para iterar sobre o map de registradores é mais limpo.
        for (const auto &pairReg : registers) {
            cout << pairReg.first << " = " << pairReg.second << endl;
        }
        cout << "---------------------------------\n";
    }

    // Imprime o estado detalhado de todos os componentes do simulador.
    // Essencial para depuração e para entender o comportamento ciclo a ciclo.
    void printStatus() {
        cout << "\n==== Ciclo " << cycle << " ====" << endl;

        // Tabela de Status das Instruções: mostra o progresso de cada instrução.
        cout << "\nInstrucoes:" << endl;
        printf("---------------------------------------------------------------------------------\n");
        printf("| %-1s | %-18s | %-7s | %-9s | %-11s | %-11s |\n", "#", "Instrucao", "Emissao", "Exec Comp", "WriteResult", "Commit");
        printf("---------------------------------------------------------------------------------\n");
        for (size_t i = 0; i < instructions.size(); ++i) {
            const Instruction &inst = instructions[i];
            string instStr; // Para formatar a instrução como string.
            switch (inst.type) { // Constrói a string da instrução.
                case ADD: instStr = "ADD " + inst.dest + "," + inst.src1 + "," + inst.src2; break;
                case SUB: instStr = "SUB " + inst.dest + "," + inst.src1 + "," + inst.src2; break;
                case MUL: instStr = "MUL " + inst.dest + "," + inst.src1 + "," + inst.src2; break;
                case DIV: instStr = "DIV " + inst.dest + "," + inst.src1 + "," + inst.src2; break;
                case LOAD: instStr = "LOAD " + inst.dest + "," + inst.src1 + "(" + inst.src2 + ")"; break;
                case STORE: instStr = "STORE " + inst.src1 + "," + inst.dest + "(" + inst.src2 + ")"; break;
                default: instStr = "INVALID"; break;
            }
            // Imprime os ciclos de cada estágio. "-" se ainda não ocorreu.
            printf("| %-1zu | %-18s | %-7s | %-9s | %-11s | %-11s |\n", i, instStr.c_str(),
                (inst.issue != -1 ? to_string(inst.issue).c_str() : "-"),
                (inst.execComp != -1 ? to_string(inst.execComp).c_str() : "-"),
                (inst.writeResult != -1 ? to_string(inst.writeResult).c_str() : "-"),
                (inst.commitCycle != -1 ? to_string(inst.commitCycle).c_str() : "-"));
        }
        printf("---------------------------------------------------------------------------------\n");

        // Tabela de Estações de Reserva (formato unificado para todos os tipos).
        // Strings de formatação para consistência.
        const char *rsTableFormat = "| %-1zu | %-4s | %-5s | %-5s | %-5s | %-7s | %-7s | %-10s | %-3s | %-7s |\n";
        const char *rsTableHeader = "| # | Busy | Op    | Vj    | Vk    | Qj(ROB#)| Qk(ROB#)| Dest(ROB#) | A   | InstIdx |\n";
        string rsLineSeparator =    "-------------------------------------------------------------------------------------";

        // Lambda para imprimir um grupo de RSs. Evita repetição de código.
        auto printRSGroup = [&](const string &name, const vector<ReservationStation> &rsGroup) {
            cout << "\nEstacoes de Reserva " << name << ":" << endl << rsLineSeparator << endl << rsTableHeader << rsLineSeparator << endl;
            for (size_t i = 0; i < rsGroup.size(); ++i) {
                const auto &rs = rsGroup[i];
                string opStr; // String para o tipo de operação na RS.
                if(rs.busy) switch(rs.op){ case ADD: opStr="ADD"; break; case SUB: opStr="SUB"; break; case MUL: opStr="MUL"; break; case DIV: opStr="DIV"; break; case LOAD: opStr="LOAD"; break; case STORE: opStr="STORE"; break; default: opStr="???"; }
                // Imprime os campos da RS. Mostra "-" se não aplicável ou não pronto.
                printf(rsTableFormat, i, (rs.busy ? "Sim" : "Nao"), opStr.c_str(),
                    (rs.busy && rs.Qj.empty() ? to_string(rs.Vj).c_str() : "-"), // Vj só se Qj estiver vazio.
                    (rs.busy && rs.Qk.empty() ? to_string(rs.Vk).c_str() : "-"), // Vk só se Qk estiver vazio.
                    (rs.busy && !rs.Qj.empty() ? rs.Qj.c_str() : "-"), // Qj se estiver esperando.
                    (rs.busy && !rs.Qk.empty() ? rs.Qk.c_str() : "-"), // Qk se estiver esperando.
                    (rs.busy ? rs.destRobIndex.c_str() : "-"),
                    // Campo 'A' (offset) é relevante apenas para LOAD/STORE.
                    (rs.busy && (rs.op == LOAD || rs.op == STORE) ? to_string(rs.A).c_str() : "-"),
                    (rs.busy && rs.instructionIndex != -1 ? to_string(rs.instructionIndex).c_str() : "-"));
            }
            cout << rsLineSeparator << endl;
        };
        // Chama a lambda para cada grupo de RS.
        printRSGroup("ADD/SUB", addRS); printRSGroup("MUL/DIV", mulRS);
        printRSGroup("LOAD", loadRS); printRSGroup("STORE", storeRS);

        // Tabela do Reorder Buffer (ROB).
        cout << "\nReorder Buffer (ROB): Head=" << robHead << ", Tail=" << robTail << ", Available=" << robEntriesAvailable << endl;
        const char *robTableFormat = "| %-4d | %-4s | %-7s | %-5s | %-11s | %-7s | %-6s | %-5s | %-7s |\n";
        cout << "------------------------------------------------------------------------------------------" << endl;
        cout << "| ROB# | Busy | InstIdx | Type  | State       | DestReg | ValRdy | Value | Address |" << endl;
        cout << "------------------------------------------------------------------------------------------" << endl;
        for (int i = 0; i < ROB_SIZE; ++i) { // Itera por todas as entradas do ROB.
            const auto &entry = rob[i];
            // Converte enums para strings para facilitar a leitura.
            string typeStr = entry.busy ? (entry.type == ADD ? "ADD" : entry.type == SUB ? "SUB" : entry.type == MUL ? "MUL" : entry.type == DIV ? "DIV" : entry.type == LOAD ? "LOAD" : entry.type == STORE ? "STORE" : "INV") : "---";
            string stateStr = entry.busy ? (entry.state == ROB_ISSUE ? "Issue" : entry.state == ROB_EXECUTE ? "Execute" : entry.state == ROB_WRITERESULT ? "WriteRes" : "Empty") : "---";
            string value_s = (entry.busy && entry.valueReady) ? to_string(entry.value) : "-";
            // Endereço só é relevante para LOAD/STORE e se já foi calculado.
            string address_s = (entry.busy && (entry.type == LOAD || entry.type == STORE) && entry.address != 0 ) ? to_string(entry.address) : "-";
            // Caso especial: STORE pode estar em WriteResult (endereço pronto) mas com dado pendente.
            if (entry.busy && entry.type == STORE && !entry.valueReady && entry.state == ROB_WRITERESULT) value_s = "(pend)";

            printf(robTableFormat, i, (entry.busy ? "Sim" : "Nao"),
                (entry.busy && entry.instructionIndex != -1 ? to_string(entry.instructionIndex).c_str() : "-"),
                typeStr.c_str(), stateStr.c_str(),
                (entry.busy ? entry.destinationRegister.c_str() : "-"),
                (entry.busy ? (entry.valueReady ? "Sim" : "Nao") : "-"),
                value_s.c_str(), address_s.c_str());
        }
        cout << "------------------------------------------------------------------------------------------" << endl;

        // Tabela de Status dos Registradores: mostra quais registradores estão aguardando resultados.
        cout << "\nRegister Status:" << endl;
        printf("---------------------\n");
        printf("| Reg | Busy | ROB# |\n");
        printf("---------------------\n");
        bool anyRegBusy = false; // Flag para verificar se algum registrador está ocupado.
        for (const auto &pair : regStatus) { // Itera sobre o map de status de registradores.
            if (pair.second.busy) { // Imprime apenas os que estão ocupados.
                anyRegBusy = true;
                printf("| %-3s | %-4s | %-4s |\n",
                    pair.first.c_str(), "Sim", to_string(pair.second.robIndex).c_str());
            }
        }
        if (!anyRegBusy) { // Se nenhum estiver ocupado, imprime uma linha indicando isso.
             printf("| --- | Nao  | -    |\n");
        }
        printf("---------------------\n");
    }
}; // Fim da classe TomasuloSimulator

// Função principal: ponto de entrada do programa.
// Configura e executa a simulação.
int main() {
    // Cria uma instância do simulador.
    // É possível passar tamanhos customizados para RSs e ROB, ex:
    // TomasuloSimulator simulator(3, 2, 3, 3, 8); // 3 AddRS, 2 MulRS, 3 LoadRS, 3 StoreRS, ROB com 8 entradas.
    TomasuloSimulator simulator; // Usa os tamanhos padrão definidos no construtor.

    string filename; // Para o nome do arquivo de instruções.
    cout << "Digite o nome do arquivo de instrucoes: ";
    cin >> filename; // Solicita o nome do arquivo ao usuário.

    // Tenta carregar as instruções do arquivo.
    if (!simulator.loadInstructions(filename)) {
        cerr << "Falha ao carregar instrucoes. Finalizando." << endl;
        return 1; // Encerra com código de erro.
    }

    // Loop principal da simulação: continua até todas as instruções serem cometidas.
    while (!simulator.isSimulationComplete()) {
        simulator.printStatus();    // Imprime o estado atual do simulador.
        simulator.stepSimulation(); // Avança um ciclo na simulação.

        cout << "\nAvancar para o proximo ciclo? [Pressione ENTER]";
        // Limpeza do buffer de entrada é importante aqui para que o cin.get()
        // aguarde uma nova entrada do usuário, em vez de consumir um '\n' pendente.
        if(cin.peek() == '\n') cin.ignore(); // Se o próximo char é '\n', consome ele.
        else cin.ignore(numeric_limits<streamsize>::max(), '\n'); // Consome tudo até o próximo '\n'.
        cin.get(); // Aguarda o usuário pressionar ENTER.
    }

    // Simulação concluída.
    cout << "\n=== Simulacao concluida no ciclo " << simulator.getCurrentCycle() -1 << " ===" << endl; // -1 porque cycle é incrementado no final de stepSimulation.
    simulator.printStatus();      // Imprime o estado final detalhado.
    simulator.printRegisters();   // Imprime os valores finais dos registradores.
    return 0; // Encerra com sucesso.
}