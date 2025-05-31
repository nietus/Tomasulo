// Simulador do Algoritmo de Tomasulo

#include <iostream>  // Para entrada e saída padrão (cout, cin)
#include <fstream>   // Para manipulação de arquivos (ifstream)
#include <vector>    // Para usar vetores dinâmicos (std::vector)
#include <string>    // Para usar strings (std::string)
#include <sstream>   // Para manipulação de strings como streams (istringstream, stringstream)
#include <map>       // Para usar mapas (std::map)
#include <queue>     // Para usar filas (std::queue)
#include <iomanip>   // Para manipulação da formatação de saída (setw, left)
#include <limits>    // Para std::numeric_limits (usado para limpar buffer de entrada)
#include <cstdlib>   // Para funções como atoi

using namespace std;

// Definicao de tipos de instrucoes suportadas
enum InstructionType
{
    ADD,
    SUB,
    MUL,
    DIV,
    LOAD,
    STORE,
    INVALID  // Tipo de instrução inválido ou não reconhecido
};

// Estrutura para representar uma instrucao
// Contém informações sobre a operação e os ciclos de cada estágio no pipeline de Tomasulo
struct Instruction
{
    InstructionType type;
    string dest;    // Registrador de destino
    string src1;    // Primeiro operando fonte (registrador ou offset para L.D/S.D, ou dado para S.D)
    string src2;    // Segundo operando fonte (registrador ou registrador base para L.D/S.D)
    // Variáveis para rastreamento de ciclos de execucao
    int issue;      // Ciclo em que a instrucao foi emitida
    int execComp;   // Ciclo em que a execucao foi concluida
    int writeResult;    // Ciclo em que o resultado da instrução foi escrito no Common Data Bus

    // Construtor - inicializa os ciclos com -1 para indicar que ainda não ocorreram
    Instruction() : issue(-1), execComp(-1), writeResult(-1) {}
};

// Estrutura para representar uma entrada na Estação de Reserva (Reservation Station - RS)
// As RSs são buffers que guardam as instruções que estão aguardando operandos ou unidades funcionais
struct ReservationStation
{
    bool busy;            // Indica se a estação de reserva está ocupada por uma instrução
    InstructionType op;   // Operação a ser executada (ADD, MUL, etc.)
    int Vj, Vk;           // Valores dos operandos fonte. Vj é para src1, Vk para src2 (ou registrador base em L/S)
    string Qj, Qk;        // Nomes das estações de reserva que produzirão os operandos Vj e Vk, respectivamente.
                          // Se o valor já estiver disponível, Qj/Qk estarão vazios.
                          // Isso implementa a renomeação de registradores e o encaminhamento de dados.
    string dest;          // Nome da RS de destino (tag) ou registrador de destino para instruções aritméticas/LOAD.
                          // Para STORE, este campo não representa um registrador de destino no banco.
    int A;                // Campo de endereço. Usado para L.D/S.D para armazenar o offset.
                          // O endereço efetivo será calculado como A + Vk (offset + valor do reg. base).
    int instructionIndex; // Índice da instrução original no vetor 'instructions' associada a esta RS.

    // Construtor - inicializa a RS como não ocupada e campos com valores padrão.
    ReservationStation() : busy(false), op(INVALID), Vj(0), Vk(0), A(0), instructionIndex(-1) {}
};

// Estrutura para representar o status de um registrador no Banco de Registradores
// Usado para implementar a renomeação de registradores.
struct RegisterStatus
{
    bool busy;          // Indica se o registrador está aguardando um resultado de alguma RS.
    string reorderName; // Nome da estação de reserva (tag) que produzirá o resultado para este registrador.
                        // Se o registrador não estiver busy, este campo não é relevante.

    // Construtor padrão: inicializa o registrador como não ocupado.
    RegisterStatus() : busy(false), reorderName("") {}
};

// Classe principal do simulador
class TomasuloSimulator
{
private:
    // --- Componentes do Processador Simulado ---
    vector<Instruction> instructions;      // Vetor contendo todas as instruções lidas do arquivo
    vector<ReservationStation> addRS;      // Estações de Reserva para unidades de Adição/Subtração
    vector<ReservationStation> mulRS;      // Estações de Reserva para unidades de Multiplicação/Divisão
    vector<ReservationStation> loadRS;     // Estações de Reserva (Buffers de Load) para instruções LOAD
    vector<ReservationStation> storeRS;    // Estações de Reserva (Buffers de Store) para instruções STORE

    map<string, int> registers;            // Banco de Registradores (ex: "F0" -> valor_inteiro)
    map<string, RegisterStatus> regStatus; // Tabela de Status dos Registradores (para renomeação)

    int memory[1024]; // Memória simulada (array de inteiros)
    int cycle;        // Contador do ciclo atual da simulação
    int nextInstructionIndex; // Índice da próxima instrução a ser emitida (issue)

    // Latências das operações (em ciclos de clock)
    // Definem quantos ciclos cada tipo de operação leva para executar após ter os operandos.
    const int ADD_LATENCY = 2;
    const int MUL_LATENCY = 10;
    const int DIV_LATENCY = 40;
    const int LOAD_LATENCY = 2;  // Latência para ler da memória (após endereço calculado)
    const int STORE_LATENCY = 2; // Latência para escrever na memória (após endereço e dado disponíveis)

    // Estrutura para rastrear instruções que estão atualmente em execução nas unidades funcionais
    struct ExecutingInstruction
    {
        int rsIndex;          // Índice da RS na sua respectiva lista (addRS, mulRS, etc.)
        string rsType;        // Tipo da RS ("ADD", "MUL", "LOAD", "STORE")
        int remainingCycles;  // Ciclos restantes para completar a execução
        int instructionIndex; // Índice da instrução original
    };

    vector<ExecutingInstruction> executingInstructions; // Lista de instruções em execução
    queue<int> completedInstructions; // Fila de instruções cuja execução terminou e aguardam para escrever no CDB
                                       // O CDB é um recurso compartilhado, geralmente apenas uma escrita por ciclo.

    // Encontra uma estação de reserva livre para o tipo de instrução especificado.
    // Retorna um par: {índice da RS livre, tipo da RS como string} ou {-1, ""} se nenhuma estiver livre.
    pair<int, string> findFreeRS(InstructionType type)
    {
        if (type == ADD || type == SUB)
        {
            for (size_t i = 0; i < addRS.size(); i++)
            {
                if (!addRS[i].busy)
                {
                    return make_pair(static_cast<int>(i), "ADD");
                }
            }
        }
        else if (type == MUL || type == DIV)
        {
            for (size_t i = 0; i < mulRS.size(); i++)
            {
                if (!mulRS[i].busy)
                {
                    return make_pair(static_cast<int>(i), "MUL");
                }
            }
        }
        else if (type == LOAD)
        {
            for (size_t i = 0; i < loadRS.size(); i++)
            {
                if (!loadRS[i].busy)
                {
                    return make_pair(static_cast<int>(i), "LOAD");
                }
            }
        }
        else if (type == STORE)
        {
            for (size_t i = 0; i < storeRS.size(); i++)
            {
                if (!storeRS[i].busy)
                {
                    return make_pair(static_cast<int>(i), "STORE");
                }
            }
        }

        return make_pair(-1, ""); // Nenhuma RS livre encontrada
    }

    // Estágio de Emissão (Issue) do pipeline de Tomasulo.
    // Tenta emitir a próxima instrução da fila para uma Estação de Reserva.
    bool issueInstruction()
    {
        // Verifica se todas as instruções já foram emitidas
        if (nextInstructionIndex >= static_cast<int>(instructions.size()))
        {
            return false; // Nenhuma nova instrução para emitir
        }

        Instruction &inst = instructions[static_cast<size_t>(nextInstructionIndex)]; // Pega a próxima instrução
        pair<int, string> result = findFreeRS(inst.type); // Tenta encontrar uma RS livre
        int rsIndex = result.first;
        string rsType = result.second;

        // Se não houver RS livre, a instrução não pode ser emitida neste ciclo (emperramento estrutural)
        if (rsIndex == -1)
        {
            return false; 
        }

        inst.issue = cycle; // Marca o ciclo de emissão da instrução

        ReservationStation *rs = NULL; // Ponteiro para a RS que será utilizada

        // Obtém o ponteiro para a RS correta baseado no tipo
        if (rsType == "ADD") rs = &addRS[static_cast<size_t>(rsIndex)];
        else if (rsType == "MUL") rs = &mulRS[static_cast<size_t>(rsIndex)];
        else if (rsType == "LOAD") rs = &loadRS[static_cast<size_t>(rsIndex)];
        else if (rsType == "STORE") rs = &storeRS[static_cast<size_t>(rsIndex)];
        
        if (rs == NULL) return false; // Segurança, não deveria acontecer se rsIndex != -1

        // Configura a Estação de Reserva com os dados da instrução
        rs->busy = true;
        rs->op = inst.type;
        rs->instructionIndex = nextInstructionIndex;

        // Lógica para buscar operandos e configurar campos Vj, Vk, Qj, Qk, A
        if (inst.type == LOAD)
        {
            // Para LOAD:
            // inst.dest é o registrador destino (ex: F2)
            // inst.src1 é o offset (ex: "100")
            // inst.src2 é o registrador base (ex: "F0")
            rs->dest = inst.dest;             // Registrador onde o valor carregado será armazenado
            rs->A = atoi(inst.src1.c_str());  // Offset para o cálculo do endereço

            // Verifica o status do registrador base (inst.src2)
            if (regStatus[inst.src2].busy) // Se o registrador base ainda não está pronto
            {
                rs->Qk = regStatus[inst.src2].reorderName; // Espera pela RS que produzirá o valor do reg. base
            }
            else
            {
                rs->Vk = registers[inst.src2]; // Valor do registrador base está disponível
                rs->Qk = "";                   // Não precisa esperar
            }
            rs->Qj = ""; // LOAD não tem um primeiro operando de dados como ADD/SUB
        }
        else if (inst.type == STORE)
        {
            // Para STORE:
            // inst.src1 é o registrador com o dado a ser armazenado (ex: F2)
            // inst.dest é o offset (ex: "200") parseado de "offset(Rbase)"
            // inst.src2 é o registrador base (ex: "F0") parseado de "offset(Rbase)"
            rs->A = atoi(inst.dest.c_str()); // Offset para o cálculo do endereço. inst.dest aqui é o offset.
            rs->dest = ""; // STORE não escreve em um registrador de destino no banco de registradores.
                           // O "destino" é a memória. Este campo na RS pode ser ignorado ou usado para debug.

            // Verifica o status do registrador de dados (inst.src1)
            if (regStatus[inst.src1].busy)
            {
                rs->Qj = regStatus[inst.src1].reorderName; // Espera pela RS que produzirá o dado
            }
            else
            {
                rs->Vj = registers[inst.src1]; // Dado está disponível
                rs->Qj = "";
            }

            // Verifica o status do registrador base (inst.src2)
            if (regStatus[inst.src2].busy)
            {
                rs->Qk = regStatus[inst.src2].reorderName; // Espera pela RS que produzirá o valor do reg. base
            }
            else
            {
                rs->Vk = registers[inst.src2]; // Valor do registrador base está disponível
                rs->Qk = "";
            }
        }
        else // Para instruções aritméticas (ADD, SUB, MUL, DIV)
        {
            rs->dest = inst.dest; // Registrador de destino da operação aritmética

            // Verifica o status do primeiro operando fonte (inst.src1)
            if (regStatus[inst.src1].busy)
            {
                rs->Qj = regStatus[inst.src1].reorderName; // Espera pela RS que produzirá Vj
            }
            else
            {
                rs->Vj = registers[inst.src1]; // Valor de src1 está disponível
                rs->Qj = "";
            }

            // Verifica o status do segundo operando fonte (inst.src2)
            if (regStatus[inst.src2].busy)
            {
                rs->Qk = regStatus[inst.src2].reorderName; // Espera pela RS que produzirá Vk
            }
            else
            {
                rs->Vk = registers[inst.src2]; // Valor de src2 está disponível
                rs->Qk = "";
            }
        }

        // Atualiza o status do registrador de destino (renomeação de registrador)
        // Exceto para STORE, que não escreve em um registrador de destino.
        // O registrador de destino agora apontará para esta RS.
        if (inst.type != STORE)
        {
            regStatus[inst.dest].busy = true;
            stringstream ss; // Usado para converter o índice da RS (int) para string
            ss << rsIndex;
            regStatus[inst.dest].reorderName = rsType + ss.str(); // Tag da RS (ex: "ADD0", "LOAD2")
        }

        nextInstructionIndex++; // Avança para a próxima instrução a ser emitida
        return true;
    }

    
    // Verifica estacoes prontas para execucao e inicia execucao
    void startExecution()
    {
        // Verifica ADD/SUB
        for (size_t i = 0; i < addRS.size(); i++)
        {
            if (addRS[i].busy && addRS[i].Qj.empty() && addRS[i].Qk.empty())
            {
                bool alreadyExecuting = false;

                // Verifica se ja esta executando
                for (size_t j = 0; j < executingInstructions.size(); j++)
                {
                    if (executingInstructions[j].rsType == "ADD" && executingInstructions[j].rsIndex == static_cast<int>(i))
                    {
                        alreadyExecuting = true;
                        break;
                    }
                }

                if (!alreadyExecuting)
                {
                    ExecutingInstruction exec;
                    exec.rsIndex = static_cast<int>(i);
                    exec.rsType = "ADD";
                    exec.instructionIndex = addRS[i].instructionIndex;

                    // Define latência baseada no tipo de operacao
                    if (addRS[i].op == ADD || addRS[i].op == SUB)
                    {
                        exec.remainingCycles = ADD_LATENCY;
                    }

                    executingInstructions.push_back(exec);
                }
            }
        }

        // Verifica MUL/DIV
        for (size_t i = 0; i < mulRS.size(); i++)
        {
            if (mulRS[i].busy && mulRS[i].Qj.empty() && mulRS[i].Qk.empty())
            {
                bool alreadyExecuting = false;

                // Verifica se ja esta executando
                for (size_t j = 0; j < executingInstructions.size(); j++)
                {
                    if (executingInstructions[j].rsType == "MUL" && executingInstructions[j].rsIndex == static_cast<int>(i))
                    {
                        alreadyExecuting = true;
                        break;
                    }
                }

                if (!alreadyExecuting)
                {
                    ExecutingInstruction exec;
                    exec.rsIndex = static_cast<int>(i);
                    exec.rsType = "MUL";
                    exec.instructionIndex = mulRS[i].instructionIndex;

                    // Define latência baseada no tipo de operacao
                    if (mulRS[i].op == MUL)
                    {
                        exec.remainingCycles = MUL_LATENCY;
                    }
                    else if (mulRS[i].op == DIV)
                    {
                        exec.remainingCycles = DIV_LATENCY;
                    }

                    executingInstructions.push_back(exec);
                }
            }
        }

        // Verifica LOAD
        for (size_t i = 0; i < loadRS.size(); i++)
        {
            if (loadRS[i].busy && loadRS[i].Qj.empty() && loadRS[i].Qk.empty())
            {
                bool alreadyExecuting = false;

                // Verifica se ja esta executando
                for (size_t j = 0; j < executingInstructions.size(); j++)
                {
                    if (executingInstructions[j].rsType == "LOAD" && executingInstructions[j].rsIndex == static_cast<int>(i))
                    {
                        alreadyExecuting = true;
                        break;
                    }
                }

                if (!alreadyExecuting)
                {
                    ExecutingInstruction exec;
                    exec.rsIndex = static_cast<int>(i);
                    exec.rsType = "LOAD";
                    exec.instructionIndex = loadRS[i].instructionIndex;
                    exec.remainingCycles = LOAD_LATENCY;

                    executingInstructions.push_back(exec);
                }
            }
        }

        // Verifica STORE
        for (size_t i = 0; i < storeRS.size(); i++)
        {
            if (storeRS[i].busy && storeRS[i].Qj.empty() && storeRS[i].Qk.empty())
            {
                bool alreadyExecuting = false;

                // Verifica se ja esta executando
                for (size_t j = 0; j < executingInstructions.size(); j++)
                {
                    if (executingInstructions[j].rsType == "STORE" && executingInstructions[j].rsIndex == static_cast<int>(i))
                    {
                        alreadyExecuting = true;
                        break;
                    }
                }

                if (!alreadyExecuting)
                {
                    ExecutingInstruction exec;
                    exec.rsIndex = static_cast<int>(i);
                    exec.rsType = "STORE";
                    exec.instructionIndex = storeRS[i].instructionIndex;
                    exec.remainingCycles = STORE_LATENCY;

                    executingInstructions.push_back(exec);
                }
            }
        }
    }

    // Estágio de Execução (Execute) - Avanço da Execução
    // Para todas as instruções que estão na lista 'executingInstructions',
    // decrementa o contador de ciclos restantes.
    void advanceExecution()
    {
        for (size_t i = 0; i < executingInstructions.size(); /* i é incrementado manualmente ou não */)
        {
            executingInstructions[i].remainingCycles--; // Decrementa ciclo restante

            if (executingInstructions[i].remainingCycles <= 0) // Execução concluída?
            {
                // Marca o ciclo de conclusão da execução na estrutura da instrução original
                instructions[static_cast<size_t>(executingInstructions[i].instructionIndex)].execComp = cycle;

                // Adiciona à fila de instruções que completaram a execução e aguardam o CDB
                completedInstructions.push(executingInstructions[i].instructionIndex);

                // Remove da lista de instruções em execução
                // Ao remover um elemento de um vetor pelo índice, o tamanho do vetor muda
                // e os elementos subsequentes são deslocados. Por isso, não incrementamos 'i' aqui.
                executingInstructions.erase(executingInstructions.begin() + static_cast<long>(i));
            }
            else
            {
                i++; // Avança para a próxima instrução em execução se esta não terminou
            }
        }
    }

    // Estágio de Escrita do Resultado (Write Result / Write Back)
    // Processa uma instrução da fila 'completedInstructions' por ciclo (simulando um CDB).
    // O resultado é escrito no banco de registradores (se aplicável) e transmitido
    // para todas as RSs que possam estar esperando por este resultado.
    void processWriteBack()
    {
        if (completedInstructions.empty()) // Se não há instruções completas para escrever
        {
            return;
        }

        int instIndex = completedInstructions.front(); // Pega a primeira instrução da fila (FIFO)
        completedInstructions.pop();                   // Remove da fila

        Instruction &inst = instructions[static_cast<size_t>(instIndex)]; // Referência para a instrução original
        inst.writeResult = cycle; // Marca o ciclo em que o resultado foi escrito

        ReservationStation *rs = NULL; // Ponteiro para a RS que executou a instrução
        string rsName;                 // Nome da RS (tag, ex: "ADD0") que produziu o resultado

        // Encontra a RS correspondente à instrução que completou
        // (Este bloco poderia ser otimizado se a RS soubesse seu próprio nome completo ou
        //  se a informação da RS estivesse mais diretamente ligada à 'completedInstructions')
        // Primeiro, procura em addRS
        for (size_t i = 0; i < addRS.size(); i++)
        {
            if (addRS[i].busy && addRS[i].instructionIndex == instIndex)
            {
                rs = &addRS[i];
                stringstream ss; ss << i; rsName = "ADD" + ss.str();
                break;
            }
        }
        // Se não encontrou, procura em mulRS
        if (rs == NULL)
        {
            for (size_t i = 0; i < mulRS.size(); i++)
            {
                if (mulRS[i].busy && mulRS[i].instructionIndex == instIndex)
                {
                    rs = &mulRS[i];
                    stringstream ss; ss << i; rsName = "MUL" + ss.str();
                    break;
                }
            }
        }
        // Se não encontrou, procura em loadRS
        if (rs == NULL)
        {
            for (size_t i = 0; i < loadRS.size(); i++)
            {
                if (loadRS[i].busy && loadRS[i].instructionIndex == instIndex)
                {
                    rs = &loadRS[i];
                    stringstream ss; ss << i; rsName = "LOAD" + ss.str();
                    break;
                }
            }
        }
        // Se não encontrou, procura em storeRS
        if (rs == NULL)
        {
            for (size_t i = 0; i < storeRS.size(); i++)
            {
                if (storeRS[i].busy && storeRS[i].instructionIndex == instIndex)
                {
                    rs = &storeRS[i];
                    stringstream ss; ss << i; rsName = "STORE" + ss.str();
                    break;
                }
            }
        }

        // Se, por algum motivo, a RS não for encontrada, é um erro.
        if (rs == NULL)
        {
            cerr << "Erro: Nao foi possivel encontrar a estacao de reserva para a instrucao " << instIndex << endl;
            return;
        }

        int resultValue = 0; // Valor do resultado calculado/carregado
        int effectiveAddress = 0; // Endereço efetivo para L/S

        // Calcula o resultado da operação ou realiza acesso à memória
        switch (inst.type)
        {
        case ADD: resultValue = rs->Vj + rs->Vk; break;
        case SUB: resultValue = rs->Vj - rs->Vk; break;
        case MUL: resultValue = rs->Vj * rs->Vk; break;
        case DIV:
            if (rs->Vk != 0) resultValue = rs->Vj / rs->Vk;
            else { cerr << "Erro: Divisao por zero na instrucao " << instIndex << "!" << endl; resultValue = 0; }
            break;
        case LOAD:
            effectiveAddress = rs->A + rs->Vk; // Endereço = offset (A) + valor_reg_base (Vk)
            // Adicionar verificação de limites da memória se necessário: if (effectiveAddress >=0 && effectiveAddress < 1024)
            resultValue = memory[effectiveAddress];
            break;
        case STORE:
            effectiveAddress = rs->A + rs->Vk; // Endereço = offset (A) + valor_reg_base (Vk)
            // Adicionar verificação de limites da memória se necessário
            memory[effectiveAddress] = rs->Vj; // Armazena o valor Vj (dado) na memória
            // STORE não produz um 'resultValue' para registradores
            break;
        default: break; // Caso inválido
        }

        // Atualiza o banco de registradores e o status do registrador
        // Apenas para instruções que têm um registrador de destino (não STORE).
        if (inst.type != STORE)
        {
            registers[rs->dest] = resultValue; // Escreve o resultado no registrador de destino

            // Libera o status do registrador se esta RS era a que estava designada a escrever nele
            if (regStatus[rs->dest].reorderName == rsName)
            {
                regStatus[rs->dest].busy = false;
                regStatus[rs->dest].reorderName = "";
            }
        }

        // Transmite o resultado (resultValue e rsName) para todas as outras RSs
        // que possam estar esperando por ele (forwarding / wakeup).
        // Apenas se a instrução não for STORE, pois STORE não produz resultado para outras RSs esperarem.
        if (inst.type != STORE) {
             updateDependentRS(rsName, resultValue);
        }
        // Para STORE, mesmo que não haja 'resultValue' para registradores, 
        // se uma futura instrução L.D dependesse da escrita de S.D (memory disambiguation),
        // uma sinalização mais complexa seria necessária. Este simulador não trata essa dependência memória-memória.

        rs->busy = false; // Libera a Estação de Reserva
        rs->instructionIndex = -1; // Reseta o índice da instrução
        // Opcional: resetar outros campos da RS (Vj, Vk, Qj, Qk, A, dest) para um estado limpo
        rs->Qj = ""; rs->Qk = ""; rs->Vj = 0; rs->Vk = 0; rs->A = 0; rs->dest = "";
    }

    // Atualiza todas as Estações de Reserva que estavam esperando por um resultado
    // que acabou de ser disponibilizado no CDB.
    // rsName: nome da RS que produziu o resultado (ex: "ADD0").
    // result: o valor do resultado.
    void updateDependentRS(const string &producingRSName, int resultValue)
    {
        // Itera por todas as estações de reserva de todos os tipos
        vector<ReservationStation>* rs_lists[] = {&addRS, &mulRS, &loadRS, &storeRS};
        for (auto& rs_list_ptr : rs_lists) {
            for (size_t i = 0; i < rs_list_ptr->size(); ++i) {
                ReservationStation& currentRS = (*rs_list_ptr)[i];
                if (currentRS.busy) { // Apenas se a RS estiver ocupada
                    // Verifica se o primeiro operando (Qj) estava esperando por este resultado
                    if (currentRS.Qj == producingRSName) {
                        currentRS.Vj = resultValue; // Recebe o valor
                        currentRS.Qj = "";          // Não precisa mais esperar por Qj
                    }
                    // Verifica se o segundo operando (Qk) estava esperando por este resultado
                    if (currentRS.Qk == producingRSName) {
                        currentRS.Vk = resultValue; // Recebe o valor
                        currentRS.Qk = "";          // Não precisa mais esperar por Qk
                    }
                }
            }
        }
    }

public:
    // Construtor do Simulador Tomasulo
    // Inicializa as contagens de RS, registradores e memória.
    TomasuloSimulator(int addRSCount = 3, int mulRSCount = 2, int loadRSCount = 3, int storeRSCount = 3)
    {
        // Inicializa o número especificado de estações de reserva para cada tipo
        addRS.resize(static_cast<size_t>(addRSCount));
        mulRS.resize(static_cast<size_t>(mulRSCount));
        loadRS.resize(static_cast<size_t>(loadRSCount));
        storeRS.resize(static_cast<size_t>(storeRSCount));

        // Inicializa os registradores (F0-F31) com um valor padrão (10)
        // e seus status como não-ocupados.
        for (int i = 0; i < 32; i++)
        {
            stringstream ss;
            ss << "F" << i; // Cria o nome do registrador (F0, F1, ...)
            string regName = ss.str();
            registers[regName] = 10; // Valor inicial
            regStatus[regName] = RegisterStatus(); // Status inicial (não ocupado)
        }

        // Inicializa a memória com valores (ex: memory[i] = i) para facilitar testes
        for (int i = 0; i < 1024; i++)
        {
            memory[i] = i;
        }

        cycle = 0;                // Inicia a simulação no ciclo 0
        nextInstructionIndex = 0; // Começa emitindo a primeira instrução (índice 0)
    }

    // Carrega as instruções de um arquivo de texto.
    // Retorna true se bem-sucedido, false caso contrário.
    bool loadInstructions(const string &filename)
    {
        ifstream file(filename.c_str()); // Abre o arquivo para leitura
        if (!file.is_open())
        {
            cerr << "Erro ao abrir arquivo: " << filename << endl;
            return false;
        }

        string line;
        // Lê o arquivo linha por linha
        while (getline(file, line))
        {
            // Ignora linhas vazias ou comentários (iniciadas com '#')
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            istringstream iss(line); // Usa istringstream para parsear a linha
            string op, op1, op2, op3_or_mem; // op1 é dest para arith/load, src1 para store. op2 é src1 ou offset, etc.

            iss >> op >> op1; // Lê a operação e o primeiro operando/destino

            // Remove vírgula do primeiro operando/destino, se houver
            if (!op1.empty() && op1[op1.length()-1] == ',')
            {
                op1 = op1.substr(0, op1.length()-1);
            }

            Instruction inst; // Cria uma nova instrução

            // Determina o tipo da instrução e parseia os operandos restantes
            if (op == "ADD" || op == "SUB" || op == "MUL" || op == "DIV")
            {
                if (op == "ADD") inst.type = ADD;
                else if (op == "SUB") inst.type = SUB;
                else if (op == "MUL") inst.type = MUL;
                else if (op == "DIV") inst.type = DIV;

                iss >> op2 >> op3_or_mem; // Lê src1 e src2
                // Remove vírgula de src1 (op2), se houver
                if (!op2.empty() && op2[op2.length()-1] == ',')
                {
                    op2 = op2.substr(0, op2.length()-1);
                }
                inst.dest = op1;    // Registrador destino
                inst.src1 = op2;    // Primeiro operando fonte
                inst.src2 = op3_or_mem; // Segundo operando fonte
            }
            else if (op == "L.D" || op == "LOAD")
            {
                inst.type = LOAD;
                inst.dest = op1; // Registrador destino (ex: F2 de "L.D F2, 100(F0)")
                iss >> op2;      // Lê a referência de memória (ex: "100(F0)")
                
                // Parseia "offset(registrador_base)"
                size_t openParen = op2.find('(');
                size_t closeParen = op2.find(')');
                if (openParen != string::npos && closeParen != string::npos)
                {
                    inst.src1 = op2.substr(0, openParen); // Offset (ex: "100")
                    inst.src2 = op2.substr(openParen + 1, closeParen - openParen - 1); // Registrador base (ex: "F0")
                } else {
                     cerr << "Formato de memoria invalido para LOAD: " << op2 << endl; continue;
                }
            }
            else if (op == "S.D" || op == "STORE")
            {
                inst.type = STORE;
                // Em "S.D F2, 100(F0)":
                // op1 é F2 (registrador fonte do dado)
                // op2 será "100(F0)" (referência de memória)
                inst.src1 = op1; // Registrador fonte do dado a ser armazenado
                iss >> op2;      // Lê a referência de memória
                
                size_t openParen = op2.find('(');
                size_t closeParen = op2.find(')');
                if (openParen != string::npos && closeParen != string::npos)
                {
                    inst.dest = op2.substr(0, openParen); // Offset (ex: "100") - inst.dest é usado para offset no STORE
                    inst.src2 = op2.substr(openParen + 1, closeParen - openParen - 1); // Registrador base (ex: "F0")
                } else {
                     cerr << "Formato de memoria invalido para STORE: " << op2 << endl; continue;
                }
            }
            else
            {
                cerr << "Instrucao nao reconhecida: " << op << endl;
                inst.type = INVALID;
                continue; // Pula para a próxima linha do arquivo
            }
            instructions.push_back(inst); // Adiciona a instrução parseada ao vetor de instruções
        }
        file.close(); // Fecha o arquivo
        return true;
    }

    // Verifica se a simulação foi concluída (todas as instruções escreveram seus resultados)
    bool isSimulationComplete() const
    {
        // Se todas as instruções tiveram seu 'writeResult' definido para um ciclo válido,
        // a simulação está completa.
        for (size_t i = 0; i < instructions.size(); i++)
        {
            if (instructions[i].writeResult == -1) // Se alguma instrução ainda não escreveu o resultado
            {
                return false; // Simulação não completa
            }
        }
        return true; // Todas as instruções completaram
    }

    // Avança um ciclo da simulação, executando as fases do Tomasulo.
    void stepSimulation()
    {
        // A ordem das fases é importante para simular corretamente o fluxo de dados e controle.
        // Em um hardware real, essas fases podem ocorrer em paralelo para diferentes instruções.
        // A simulação ciclo a ciclo as executa sequencialmente para determinar o estado em cada ciclo.

        // 1. Escrita de Resultados (Write Back):
        //    Processa instruções que completaram a execução no ciclo anterior e liberam o CDB.
        //    Deve vir antes do Issue para que os recursos (RS, status de reg) sejam liberados
        //    e possam ser usados por novas instruções emitidas neste mesmo ciclo.
        //    Também atualiza RSs esperando por resultados, que podem então começar a executar.
        processWriteBack();

        // 2. Emissão (Issue):
        //    Tenta emitir uma nova instrução para uma RS, se houver RS disponível.
        //    Realiza renomeação de registradores e captura de operandos.
        issueInstruction(); // Tenta emitir a próxima instrução da fila

        // 3. Início da Execução (Execute Start):
        //    Verifica RSs com operandos prontos e as envia para as unidades funcionais.
        //    Pode acontecer após o Issue, pois uma instrução emitida pode já ter operandos
        //    e começar a executar no mesmo ciclo (dependendo da latência da UF e da disponibilidade).
        //    Também considera resultados do WriteBack deste ciclo que podem ter liberado operandos.
        startExecution();

        // 4. Avanço da Execução (Execute Advance):
        //    Decrementa os ciclos restantes para instruções que já estão em execução.
        advanceExecution();

        cycle++; // Avança para o próximo ciclo da simulação
    }

    // Getter para o ciclo atual da simulação
    int getCurrentCycle() const
    {
        return cycle;
    }

    // Imprime os valores finais dos registradores ao término da simulação.
    void printRegisters() const
    {
        cout << "\nValores Finais dos Registradores:\n";
        cout << "---------------------------------\n";
        // Itera pelo mapa de registradores e imprime nome e valor
        for (map<string, int>::const_iterator it = registers.begin(); it != registers.end(); ++it)
        {
            cout << it->first << " = " << it->second << endl;
        }
        cout << "---------------------------------\n";
    }

    // Imprime o estado atual da simulação (status das instruções e das Estações de Reserva).
    // Esta função é crucial para depurar e entender o comportamento do algoritmo ciclo a ciclo.
    void printStatus()
    {
        cout << "\n";
        cout << "==== Ciclo " << cycle << " ====" << endl;

        // Imprime o status de cada instrução
        cout << "\nInstrucoes:" << endl;
        cout << "---------------------------------------------------------------" << endl;
        cout << "| # | Instrucao          | Emissao | Exec Comp | Write Result |" << endl;
        cout << "---------------------------------------------------------------" << endl;
        for (size_t i = 0; i < instructions.size(); i++)
        {
            const Instruction &inst = instructions[i];
            string instStr; // String formatada da instrução
            // Monta a string da instrução baseada no seu tipo e operandos
            switch (inst.type)
            {
            case ADD: instStr = "ADD " + inst.dest + "," + inst.src1 + "," + inst.src2; break;
            case SUB: instStr = "SUB " + inst.dest + "," + inst.src1 + "," + inst.src2; break;
            case MUL: instStr = "MUL " + inst.dest + "," + inst.src1 + "," + inst.src2; break;
            case DIV: instStr = "DIV " + inst.dest + "," + inst.src1 + "," + inst.src2; break;
            case LOAD: instStr = "LOAD " + inst.dest + "," + inst.src1 + "(" + inst.src2 + ")"; break;
            case STORE: instStr = "STORE " + inst.src1 + "," + inst.dest + "(" + inst.src2 + ")"; break; // inst.dest é offset aqui
            default: instStr = "INVALID"; break;
            }

            cout << "| " << std::right << std::setw(1) << i << " | " << std::left << setw(18) << instStr << " | ";
            
            // Converte ciclos (int) para string para impressão, mostrando "-" se não ocorreu
            stringstream ssIssue, ssExec, ssWrite;
            if (inst.issue != -1) ssIssue << inst.issue;
            if (inst.execComp != -1) ssExec << inst.execComp;
            if (inst.writeResult != -1) ssWrite << inst.writeResult;
            
            cout << setw(7) << (inst.issue != -1 ? ssIssue.str() : "-") << " | ";
            cout << setw(9) << (inst.execComp != -1 ? ssExec.str() : "-") << " | ";
            cout << setw(12) << (inst.writeResult != -1 ? ssWrite.str() : "-") << " |" << endl;
        }
        cout << "---------------------------------------------------------------" << endl;

        // Imprime o estado das Estações de Reserva ADD/SUB
        cout << "\nEstacoes de Reserva ADD/SUB:" << endl;
        // Cabeçalho da tabela de RS:
        // #: Índice da RS
        // Busy: Se está ocupada (Sim/Nao)
        // Op: Operação (ADD, SUB)
        // Vj, Vk: Valores dos operandos (se disponíveis)
        // Qj, Qk: Tags das RSs que produzirão os operandos (se esperando)
        // Dest: Tag da RS de destino ou nome do registrador destino
        // A: Campo de endereço (não usado por ADD/SUB RSs, mas impresso para consistência da tabela)
        // InstIdx: Índice da instrução original ocupando esta RS
        cout << "-----------------------------------------------------------------" << endl;
        cout << "| # | Busy | Op  | Vj  | Vk  | Qj  | Qk  | Dest | A   | InstIdx |" << endl;
        cout << "-----------------------------------------------------------------" << endl;
        for (size_t i = 0; i < addRS.size(); i++)
        {
            const ReservationStation &rs = addRS[i];
            string opStr = rs.busy ? (rs.op == ADD ? "ADD" : (rs.op == SUB ? "SUB" : "???")) : "";
            cout << "| " << setw(1) << i << " | " << setw(4) << (rs.busy ? "Sim" : "Nao") << " | " << setw(3) << opStr << " | ";
            stringstream ssVj, ssVk, ssA, ssInstIdx;
            if (rs.busy && rs.Qj.empty()) ssVj << rs.Vj;
            if (rs.busy && rs.Qk.empty()) ssVk << rs.Vk;
            if (rs.busy) ssA << rs.A; // A não é usado por ADD/SUB, será 0
            if (rs.busy && rs.instructionIndex != -1) ssInstIdx << rs.instructionIndex;
            cout << setw(3) << (rs.busy && rs.Qj.empty() ? ssVj.str() : "-") << " | ";
            cout << setw(3) << (rs.busy && rs.Qk.empty() ? ssVk.str() : "-") << " | ";
            cout << setw(3) << (rs.busy && !rs.Qj.empty() ? rs.Qj : "-") << " | ";
            cout << setw(3) << (rs.busy && !rs.Qk.empty() ? rs.Qk : "-") << " | ";
            cout << setw(4) << (rs.busy ? rs.dest : "-") << " | ";
            cout << setw(3) << (rs.busy ? ssA.str() : "-") << " | ";
            cout << setw(7) << (rs.busy && rs.instructionIndex !=-1 ? ssInstIdx.str() : "-") << " |" << endl;
        }
        cout << "-----------------------------------------------------------------" << endl;

        // Imprime o estado das Estações de Reserva MUL/DIV (lógica similar à ADD/SUB)
        cout << "\nEstacoes de Reserva MUL/DIV:" << endl;
        cout << "-----------------------------------------------------------------" << endl;
        cout << "| # | Busy | Op  | Vj  | Vk  | Qj  | Qk  | Dest | A   | InstIdx |" << endl;
        cout << "-----------------------------------------------------------------" << endl;
        for (size_t i = 0; i < mulRS.size(); i++)
        {
            const ReservationStation &rs = mulRS[i];
            string opStr = rs.busy ? (rs.op == MUL ? "MUL" : (rs.op == DIV ? "DIV" : "???")) : "";
            cout << "| " << setw(1) << i << " | " << setw(4) << (rs.busy ? "Sim" : "Nao") << " | " << setw(3) << opStr << " | ";
            stringstream ssVj, ssVk, ssA, ssInstIdx;
            if (rs.busy && rs.Qj.empty()) ssVj << rs.Vj;
            if (rs.busy && rs.Qk.empty()) ssVk << rs.Vk;
            if (rs.busy) ssA << rs.A; // A não é usado por MUL/DIV, será 0
            if (rs.busy && rs.instructionIndex != -1) ssInstIdx << rs.instructionIndex;
            cout << setw(3) << (rs.busy && rs.Qj.empty() ? ssVj.str() : "-") << " | ";
            cout << setw(3) << (rs.busy && rs.Qk.empty() ? ssVk.str() : "-") << " | ";
            cout << setw(3) << (rs.busy && !rs.Qj.empty() ? rs.Qj : "-") << " | ";
            cout << setw(3) << (rs.busy && !rs.Qk.empty() ? rs.Qk : "-") << " | ";
            cout << setw(4) << (rs.busy ? rs.dest : "-") << " | ";
            cout << setw(3) << (rs.busy ? ssA.str() : "-") << " | ";
            cout << setw(7) << (rs.busy && rs.instructionIndex != -1 ? ssInstIdx.str() : "-") << " |" << endl;
        }
        cout << "-----------------------------------------------------------------" << endl;
        
        // Imprime o estado das Estações de Reserva LOAD
        // Qj não é usado para LOAD (não tem operando fonte de dados). Vk/Qk é para o registrador base.
        // Dest é o registrador destino. A é o offset.
        cout << "\nEstacoes de Reserva LOAD:" << endl;
        cout << "-----------------------------------------------------------------" << endl;
        cout << "| # | Busy | Op  | Vj  | Vk  | Qj  | Qk  | Dest | A   | InstIdx |" << endl;
        cout << "-----------------------------------------------------------------" << endl;
        for (size_t i = 0; i < loadRS.size(); i++)
        {
            const ReservationStation &rs = loadRS[i];
            string opStr = rs.busy ? (rs.op == LOAD ? "LOAD" : "???") : "";
            cout << "| " << setw(1) << i << " | " << setw(4) << (rs.busy ? "Sim" : "Nao") << " | " << setw(3) << opStr << " | ";
            stringstream ssVj, ssVk, ssA, ssInstIdx;
            if (rs.busy && rs.Qj.empty()) ssVj << rs.Vj; // Vj não é usado por LOAD, será 0
            if (rs.busy && rs.Qk.empty()) ssVk << rs.Vk; // Vk é valor do reg. base
            if (rs.busy) ssA << rs.A;                   // A é o offset
            if (rs.busy && rs.instructionIndex != -1) ssInstIdx << rs.instructionIndex;
            cout << setw(3) << (rs.busy && rs.Qj.empty() ? ssVj.str() : "-") << " | "; // Vj (sempre - ou 0)
            cout << setw(3) << (rs.busy && rs.Qk.empty() ? ssVk.str() : "-") << " | "; // Vk (valor do reg. base)
            cout << setw(3) << (rs.busy && !rs.Qj.empty() ? rs.Qj : "-") << " | "; // Qj (sempre -)
            cout << setw(3) << (rs.busy && !rs.Qk.empty() ? rs.Qk : "-") << " | "; // Qk (tag do reg. base)
            cout << setw(4) << (rs.busy ? rs.dest : "-") << " | "; // Registrador Destino
            cout << setw(3) << (rs.busy ? ssA.str() : "-") << " | "; // Offset
            cout << setw(7) << (rs.busy && rs.instructionIndex != -1 ? ssInstIdx.str() : "-") << " |" << endl;
        }
        cout << "-----------------------------------------------------------------" << endl;

        // Imprime o estado das Estações de Reserva STORE
        // Vj/Qj é para o dado a ser armazenado. Vk/Qk é para o registrador base.
        // Dest não é usado (STORE não escreve em registrador). A é o offset.
        cout << "\nEstacoes de Reserva STORE:" << endl;
        cout << "-----------------------------------------------------------------" << endl;
        cout << "| # | Busy | Op  | Vj  | Vk  | Qj  | Qk  | Dest | A   | InstIdx |" << endl;
        cout << "-----------------------------------------------------------------" << endl;
        for (size_t i = 0; i < storeRS.size(); i++)
        {
            const ReservationStation &rs = storeRS[i];
            string opStr = rs.busy ? (rs.op == STORE ? "STORE" : "???") : "";
            cout << "| " << setw(1) << i << " | " << setw(4) << (rs.busy ? "Sim" : "Nao") << " | " << setw(3) << opStr << " | ";
            stringstream ssVj, ssVk, ssA, ssInstIdx;
            if (rs.busy && rs.Qj.empty()) ssVj << rs.Vj; // Vj é o valor do dado a ser armazenado
            if (rs.busy && rs.Qk.empty()) ssVk << rs.Vk; // Vk é valor do reg. base
            if (rs.busy) ssA << rs.A;                   // A é o offset
            if (rs.busy && rs.instructionIndex != -1) ssInstIdx << rs.instructionIndex;
            cout << setw(3) << (rs.busy && rs.Qj.empty() ? ssVj.str() : "-") << " | "; // Vj (dado)
            cout << setw(3) << (rs.busy && rs.Qk.empty() ? ssVk.str() : "-") << " | "; // Vk (valor do reg. base)
            cout << setw(3) << (rs.busy && !rs.Qj.empty() ? rs.Qj : "-") << " | "; // Qj (tag do dado)
            cout << setw(3) << (rs.busy && !rs.Qk.empty() ? rs.Qk : "-") << " | "; // Qk (tag do reg. base)
            cout << setw(4) << (rs.busy ? rs.dest : "-") << " | "; // Dest (não usado, será - ou vazio)
            cout << setw(3) << (rs.busy ? ssA.str() : "-") << " | "; // Offset
            cout << setw(7) << (rs.busy && rs.instructionIndex != -1 ? ssInstIdx.str() : "-") << " |" << endl;
        }
        cout << "-----------------------------------------------------------------" << endl;
    }
}; // Fim da classe TomasuloSimulator

// Função principal do programa
int main()
{
    TomasuloSimulator simulator; // Cria uma instância do simulador

    string filename;
    cout << "Digite o nome do arquivo de instrucoes: ";
    cin >> filename; // Pede ao usuário o nome do arquivo de instruções

    // Carrega as instruções do arquivo
    if (!simulator.loadInstructions(filename))
    {
        cerr << "Falha ao carregar instrucoes. Finalizando." << endl;
        return 1; // Encerra se houver erro ao carregar
    }

    // Loop principal da simulação: continua enquanto houver instruções não concluídas
    while (!simulator.isSimulationComplete())
    {
        simulator.printStatus(); // Imprime o estado atual da simulação
        simulator.stepSimulation(); // Avança um ciclo na simulação

        cout << "\nAvancar para o proximo ciclo? [Pressione ENTER]";
        // Limpa o buffer de entrada antes de esperar pelo Enter,
        // especialmente após um 'cin >> filename' que pode deixar um '\n'.
        cin.ignore(numeric_limits<streamsize>::max(), '\n'); 
        cin.get(); // Espera o usuário pressionar Enter
    }

    // Simulação concluída
    cout << "\n=== Simulacao concluida no ciclo " << simulator.getCurrentCycle() -1 << " ===" << endl; // -1 porque o ciclo avança antes da verificação final
    simulator.printStatus();      // Imprime o estado final
    simulator.printRegisters();   // Imprime os valores finais dos registradores

    return 0; // Fim do programa
}